# t0_test_sources_registered.cmake -- the driver for the registration check.
#
# A test source that no CMake names is compiled by nothing and run by nothing,
# while the file on disk reads as coverage. Reviewing the directory does not
# catch it: an unregistered source and a registered one look the same in a
# listing, and a passing run of a sibling test says nothing about it.
#
# It is a `cmake -P` script rather than a compiled test because the thing it
# reports on is a build that may not work. A test behind this directory's build
# fixture reports ***Not Run when a source fails to compile, which is the shape
# of silence this check exists to break.

cmake_minimum_required(VERSION 3.15)

foreach(g2RequiredArgument IN ITEMS G2_TEST_DIR)
	if(NOT DEFINED ${g2RequiredArgument}
			OR "${${g2RequiredArgument}}" STREQUAL ""
			OR "${${g2RequiredArgument}}" MATCHES "NOTFOUND$")
		message(FATAL_ERROR
			"t0_test_sources_registered: ${g2RequiredArgument} was not supplied "
			"(value: '${${g2RequiredArgument}}'). The check cannot run, and it "
			"reports that rather than passing vacuously.")
	endif()
endforeach()

if(NOT IS_DIRECTORY "${G2_TEST_DIR}")
	message(FATAL_ERROR
		"t0_test_sources_registered: G2_TEST_DIR is '${G2_TEST_DIR}', which is "
		"not a directory. A glob over a path that does not exist returns "
		"nothing, and nothing is indistinguishable here from a clean tree.")
endif()

# ---------------- the two populations
#
# One unit of the first is one test source file directly in G2_TEST_DIR. The
# glob does not descend: this directory's build fixture reads the DIRECTORY
# property TESTS, which does not descend either, so a subdirectory is a
# different problem with a different guard.

file(GLOB g2TestSourcePaths LIST_DIRECTORIES false
	"${G2_TEST_DIR}/t0_*.c"
	"${G2_TEST_DIR}/t0_*.cpp"
	"${G2_TEST_DIR}/t1_*.c"
	"${G2_TEST_DIR}/t1_*.cpp")

set(g2TestSources "")

foreach(g2Path IN LISTS g2TestSourcePaths)
	get_filename_component(g2Name "${g2Path}" NAME)
	list(APPEND g2TestSources "${g2Name}")
endforeach()

list(REMOVE_DUPLICATES g2TestSources)
list(SORT g2TestSources)
list(LENGTH g2TestSources g2TestSourceCount)

if(g2TestSourceCount EQUAL 0)
	message(FATAL_ERROR
		"t0_test_sources_registered: no test source was found in "
		"'${G2_TEST_DIR}'. Every clause below is quantified over that set, so "
		"an empty one would make all of them vacuously true and this check "
		"would report a clean tree having examined nothing.")
endif()

file(GLOB g2CMakePaths LIST_DIRECTORIES false
	"${G2_TEST_DIR}/CMakeLists.txt"
	"${G2_TEST_DIR}/*.cmake")

list(LENGTH g2CMakePaths g2CMakeFileCount)

if(g2CMakeFileCount EQUAL 0)
	message(FATAL_ERROR
		"t0_test_sources_registered: no CMake file was found in "
		"'${G2_TEST_DIR}'. With nothing to parse every source below would be "
		"reported unregistered, which is a true verdict reached for the wrong "
		"reason and would be read as a tree-wide defect.")
endif()

set(g2CMakeText "")

foreach(g2Path IN LISTS g2CMakePaths)
	file(READ "${g2Path}" g2Chunk)
	string(APPEND g2CMakeText "\n${g2Chunk}")
endforeach()

# A `#` outside a quoted argument opens a comment. Dropping the rest of the
# line keeps a worked example of a registration, written in prose, from
# counting as a call site. The leading newline this text already carries lets
# every pattern below anchor on "\n" and so on a line start, which CMake's `^`
# does not give: it matches the start of the whole string only.
string(REGEX REPLACE "#[^\n]*" "" g2CMakeText "${g2CMakeText}")

# A trailing newline so the extension match below, which requires a character
# after the extension to prove the name ended there, has one at end of text.
string(APPEND g2CMakeText "\n")

# ---------------- what counts as naming a source
#
# `add_executable` and `target_sources` hand a file to the compiler at build
# time. `add_test` is here for the third form this directory uses: a `cmake -P`
# driver given its subject as an argument and compiling it at test time. That
# source reaches a compiler too, so counting only the first two would report it
# unregistered -- an exception that would then need a name in an allowlist, when
# what it actually is is a registration this parse did not read.

string(REGEX MATCHALL
	"\n[ \t]*(add_executable|target_sources|add_test)\\([^)]*\\)"
	g2SourceCalls "${g2CMakeText}")

set(g2RegisteredSources "")

# The trailing character class is what makes the extension the END of the name.
# Without it `t0_thing.cmake` matches as far as `t0_thing.c` and a driver script
# counts as a registration of a C source that may not exist.
foreach(g2Call IN LISTS g2SourceCalls)
	string(REGEX MATCHALL "t[01]_[A-Za-z0-9_]+\\.c(pp)?[^A-Za-z0-9_]"
		g2Named "${g2Call}")

	foreach(g2Name IN LISTS g2Named)
		string(REGEX REPLACE "[^A-Za-z0-9_]$" "" g2Name "${g2Name}")
		list(APPEND g2RegisteredSources "${g2Name}")
	endforeach()
endforeach()

list(REMOVE_DUPLICATES g2RegisteredSources)
list(SORT g2RegisteredSources)
list(LENGTH g2RegisteredSources g2RegisteredSourceCount)

if(g2RegisteredSourceCount EQUAL 0)
	message(FATAL_ERROR
		"t0_test_sources_registered: the CMake in '${G2_TEST_DIR}' names no "
		"test source at all. The parse, not the tree, is what that most likely "
		"describes, and a parse that matches nothing reports every source "
		"unregistered for a reason that has nothing to do with the sources.")
endif()

# One unit here is one `add_test(NAME ...)` call whose name starts a line.
string(REGEX MATCHALL "\n[ \t]*add_test\\(NAME[ \t]+[A-Za-z0-9_.+-]+"
	g2AddTestCalls "${g2CMakeText}")

set(g2RegisteredTestNames "")

foreach(g2Call IN LISTS g2AddTestCalls)
	if(g2Call MATCHES "add_test\\(NAME[ \t]+([A-Za-z0-9_.+-]+)")
		list(APPEND g2RegisteredTestNames "${CMAKE_MATCH_1}")
	endif()
endforeach()

list(REMOVE_DUPLICATES g2RegisteredTestNames)
list(SORT g2RegisteredTestNames)
list(LENGTH g2RegisteredTestNames g2RegisteredTestNameCount)

if(g2RegisteredTestNameCount EQUAL 0)
	message(FATAL_ERROR
		"t0_test_sources_registered: the CMake in '${G2_TEST_DIR}' registers no "
		"test. As above, that describes the parse before it describes the tree.")
endif()

message(STATUS
	"t0_test_sources_registered: ${g2TestSourceCount} test source(s) in "
	"${g2CMakeFileCount} CMake file(s), naming ${g2RegisteredSourceCount} "
	"source(s) across ${g2RegisteredTestNameCount} registered test(s).")

# ---------------- clause 1: every test source is handed to a compiler

set(g2UnregisteredSources "")

foreach(g2Source IN LISTS g2TestSources)
	if(NOT "${g2Source}" IN_LIST g2RegisteredSources)
		list(APPEND g2UnregisteredSources "${g2Source}")
	endif()
endforeach()

# ---------------- clause 2: every executable built from one is run
#
# A target that compiles but that no `add_test` names is built by ctest's own
# build fixture and then never executed, so it costs the time of a test and
# reports the result of none.

string(REGEX MATCHALL "\n[ \t]*add_executable\\([^)]*\\)"
	g2ExecutableCalls "${g2CMakeText}")

set(g2UnrunExecutables "")

foreach(g2Call IN LISTS g2ExecutableCalls)
	if(NOT g2Call MATCHES "add_executable\\([ \t]*([A-Za-z0-9_.+-]+)")
		continue()
	endif()

	set(g2Target "${CMAKE_MATCH_1}")

	if(NOT g2Call MATCHES "t[01]_[A-Za-z0-9_]+\\.c(pp)?[^A-Za-z0-9_]")
		continue()
	endif()

	if(NOT "${g2Target}" IN_LIST g2RegisteredTestNames)
		list(APPEND g2UnrunExecutables "${g2Target}")
	endif()
endforeach()

list(REMOVE_DUPLICATES g2UnrunExecutables)
list(SORT g2UnrunExecutables)

# ---------------- the verdict

set(g2Failures "")

if(g2UnregisteredSources)
	string(REPLACE ";" "\n    " g2UnregisteredList "${g2UnregisteredSources}")
	string(APPEND g2Failures
		"\n  Test source(s) that no add_executable, target_sources or add_test "
		"names, so nothing compiles them and nothing runs them:\n    "
		"${g2UnregisteredList}\n"
		"  Add each to its track's own tests_*.cmake, or delete it.\n")
endif()

if(g2UnrunExecutables)
	string(REPLACE ";" "\n    " g2UnrunList "${g2UnrunExecutables}")
	string(APPEND g2Failures
		"\n  Executable(s) built from a test source that no add_test(NAME ...) "
		"names, so they are compiled and never run:\n    "
		"${g2UnrunList}\n")
endif()

if(g2Failures)
	message(FATAL_ERROR "t0_test_sources_registered FAILED:${g2Failures}")
endif()

message(STATUS
	"t0_test_sources_registered: every test source is named by the committed "
	"CMake and every executable built from one is registered as a test.")
