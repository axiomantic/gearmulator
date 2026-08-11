cmake_minimum_required(VERSION 3.15)

# THIS FILE HAS ONE OWNER: task BRD-0. See plan section 7.4.2.
#
# It is the REGISTRAR of every test under source/nord/g2/g2Lib/test/. A track
# adds its own add_test(NAME <name> ...) lines to its own per-track list below
# and to nothing else.
#
# EVERY TEST IS REGISTERED WITH THE EXACT NAME ITS TASK'S Check: LINE PASSES
# TO -R. Plan section 7.7 lints that correspondence.
#
# Every list is created empty by BRD-0.

include(tests_board.cmake)
include(tests_sched.cmake)
include(tests_chain.cmake)
include(tests_dsp.cmake)
include(tests_repo.cmake)
include(tests_plugin.cmake)
include(tests_proto.cmake)
include(tests_oracle.cmake)
include(tests_perf.cmake)
include(tests_int.cmake)

# ---------------------------------------------------------------------------
# STALE-ARTIFACT GUARD. Keep this block LAST in this file.
#
# WHAT IT DOES: before every link of every executable this directory declares,
# it deletes that executable's own output file. A link that then fails leaves
# nothing behind, so ctest cannot run a previous generation's binary and
# report a green describing code that no longer builds. CTest already does the
# right thing when a binary is ABSENT: it reports ***Not Run and counts the
# test as FAILED.
#
# IT IS INERT ON AN UP-TO-DATE TARGET. PRE_LINK runs only when the target
# actually relinks, so a no-op rebuild executes none of these commands and
# cannot touch a binary ctest is about to run.
#
# ITS EFFECT ON THIS TOOLCHAIN IS UNDEMONSTRATED. On macOS/arm64 with Apple
# clang the compiler driver already removes its own output when the link
# fails, so the stale binary this guard exists to delete is gone before the
# guard's rm could matter. Measured four ways: a scratch CMake project with
# NO guard loses its binary on a failed link; a bare `c++ -o out bad.o`, with
# no CMake and no make involved, deletes an existing `out`; that same failing
# link also deletes a PLAIN TEXT file placed at the output path, so the
# removal is neither make's nor CMake's and is not confined to a file the link
# itself produced; and the DISCRIMINATING control separates the driver from
# the linker - run the driver's own ld command line directly, with a plain
# text file at the output path, and ld exits 1 and LEAVES that file in place,
# byte for byte. So the removal is the clang DRIVER cleaning up after a failed
# link: not ld, not make, not CMake. A deliberate-break demonstration on this
# machine therefore cannot tell this guard apart from its own absence. Do not
# cite one as proof that the guard works.
#
# IT IS KEPT AS DEFENCE, NOT AS A LOAD-BEARING PART. Deleting the output on
# failure is a driver convention, not a guarantee this build may rely on. This
# repository also targets Linux and Windows, where the behaviour is UNVERIFIED,
# and a toolchain that leaves the failed output in place is exactly the case
# this block covers.
#
# THE GENERATOR IS A SECOND UNVERIFIED AXIS, and it is named here because the
# toolchain axis above is named. Everything measured for this block was
# measured under the MAKEFILE generator on macOS. PRE_LINK under the Visual
# Studio generator - which CLAUDE.md names as the current dev setup - and
# under Ninja has been observed by nobody here. Read the paragraphs above as
# silent about those two generators, not as covering them.
#
# WHAT IT COSTS IS NOT ONE rm EVERYWHERE. Under the Makefile generator it is
# one removal per relink. On the setup CLAUDE.md documents as current -
# Visual Studio 2026 in temp/cmake_vs26, with `--config Debug` as the quick
# compile-check loop - MSVC defaults to /INCREMENTAL for Debug, and
# incremental linking needs the previous .exe and .ilk. Deleting the output
# should therefore force a FULL link of every test executable on every relink
# of exactly that loop. That is documented MSVC behaviour and NOT a
# measurement taken here: nobody has timed it. It is written down so a Windows
# developer who meets slow relinks knows where to look first.
#
# A COMPILE FAILURE BYPASSES IT ENTIRELY, and that gap is OPEN. When a
# translation unit fails to compile, the link never starts, PRE_LINK never
# runs, the previous binary survives untouched, and ctest runs it and reports
# Passed. Reproduced with the guard present: invalid C++ in a test source
# takes the build to exit 2, prints zero "Discarding any previous" lines,
# leaves the binary byte-identical, and ctest still exits 0. This block does
# not close that route and must not be read as closing it. Plan section 24.6
# row W3-33 carries it, as a CARRIED DEFECT and not as a task.
#
# IT CARRIES NO MECHANISM, and the plan accepts that ONCE, in BRD-0's block.
# This is section 24.6 row W3-30's shape - an invariant with a good comment at
# the site and nothing that goes red if the whole block is deleted - and the
# SECOND of W3-30's two acceptance clauses is the one taken. The first is
# refused for a MECHANICAL reason and not for effort: the only check available
# is the coverage recipe below, which reads build.make, and ONLY the Makefile
# generator writes build.make. BRD-0's Check: requires macOS arm64, Linux
# x86-64 and Windows x86-64, where the generator is Visual Studio or Ninja and
# no such fragment exists at all, so registering that count as a test would
# turn a tri-platform check RED on Windows by construction. No portable
# mechanism exists to order, so none is ordered.
#
# ON THE INCIDENT THIS BLOCK USED TO CITE: the t0_board_surface case was NOT a
# stale-binary green. a4278548 (Aug 8) added board.cpp, board.h and
# t0_board_surface.cpp and no CMake file at all, so until f8791f9a (Aug 10)
# there was no target, no binary and nothing for ctest to run. That false
# completion lived in the task ledger, not in ctest output. Nothing in this
# repository corroborates the "Aug 8 binary" the earlier text described.
#
# It is done at PRE_LINK rather than in a test-time freshness assertion or in
# the build invocation on purpose: it is welded into the buildsystem, so it
# cannot be forgotten by someone who later runs `ctest` by hand, and it needs
# no per-target opt-in. It reads BUILDSYSTEM_TARGETS after every include
# above, so a target a future track adds to its own tests_*.cmake is covered
# on the day it is added, with no edit here.
#
# COVERAGE IS A DIAGNOSTIC A READER MAY RUN, NEVER A COUNT THIS BLOCK CLAIMS.
# The loop enumerates BUILDSYSTEM_TARGETS, which the configure step computes,
# so there is nothing here for anyone to maintain - and a number written here
# would go stale by the very mechanism this block is kept for. To see what
# generated, match the PRE_LINK line, which carries the output PATH, from the
# build directory that mirrors THIS source directory:
#
#   find CMakeFiles -maxdepth 2 -name build.make \
#     -exec grep -lE '^[[:space:]]*cd .* && .*cmake -E rm -f /' {} \; | wc -l
#
# TWO NAIVE FORMS OF THAT RECIPE LIE, both by over-reporting, and NEITHER of
# them is what the -maxdepth guards. Do NOT grep for the bare string
# `cmake -E rm -f`: every generated build.make carries the boilerplate
# `RM = <cmake> -E rm -f`, so that match returns EVERY build.make it reaches -
# including the orphan .dir directories CMake leaves behind for removed
# targets and does not prune. In a FRESH tree there are no orphans and that
# form agrees with the recipe above, which is what makes the trap quiet; the
# two separate only after a target has been removed. And do NOT run the grep
# RECURSIVELY FROM THE BUILD DIRECTORY: SCH-3's t0_clock_guard configures
# whole nested CMake projects under t0_clock_guard_work, so a recursive form
# sweeps that sub-build's fragments in as well. It does so only once that test
# has RUN, which is exactly when a reader reaches for this diagnostic.
#
# THE -maxdepth EXCLUDES NOTHING HERE, and it is kept as a tightening rather
# than as the guard. `find CMakeFiles` is what keeps the sub-build out:
# t0_clock_guard_work is a SIBLING of CMakeFiles, so no depth reaches it. And
# every build.make under this directory's CMakeFiles sits at exactly
# CMakeFiles/<target>.dir/build.make, so dropping -maxdepth moves the result
# by nothing. Measured both ways on a macOS Makefile tree: identical counts.
#
# ALSO OUT OF REACH, stated rather than hidden: someone who edits a source
# file and runs `ctest` without building at all. ctest does not build, so no
# target-declaration fix can reach that case.
#
# THE CMake FLOOR THIS BLOCK NEEDS IS 3.26, above the 3.15 the file declares,
# and cmake_minimum_required is deliberately NOT raised, because raising a
# fork's floor is a divergence. Read out of the CMake 4.3.4 docs installed
# here: add_custom_command's TARGET form gained "support for target-dependent
# generator expressions" in 3.21, which is what $<TARGET_FILE:...> in COMMAND
# is, and COMMENT accepts generator expressions only from 3.26. A `remove -f`
# shim for CMake below 3.17, where `cmake -E rm` did not yet exist, used to
# stand here; it bought a 3.15/3.16 compatibility this block does not honour
# on either count, so it is REMOVED rather than left standing as decoration.
# Below 3.26 the effect is untested here.
#
# THAT 3.26 IS RECORDED NOWHERE BUT IN THIS COMMENT. The only version
# requirement BRD-0 records in the plan is CTest 3.20 or newer, for section
# 7.7's --no-tests=error flag; the plan holds no occurrence of 3.26 or of 3.21
# at all, and REPO-6's job matrix pins CMake and CTest at "3.20 or newer",
# which is BELOW what this block needs. So a reader who goes to the plan for
# this floor does not find it, and a job built to that matrix may run a CMake
# this block is untested on. Recording it plan-side is BRD-0's work and it is
# NOT DONE. Read this paragraph as the report that the record is missing, and
# never as the record.
# ---------------------------------------------------------------------------

get_property(_g2_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)

foreach(_g2_target IN LISTS _g2_test_targets)
	get_target_property(_g2_target_type ${_g2_target} TYPE)

	if(_g2_target_type STREQUAL "EXECUTABLE")
		add_custom_command(TARGET ${_g2_target} PRE_LINK
			COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE:${_g2_target}>
			COMMENT "Discarding any previous $<TARGET_FILE_NAME:${_g2_target}> so a failed link cannot be tested")
	endif()
endforeach()

unset(_g2_target)	# CMP0124 is unset (so it behaves as OLD) and foreach() leaks the loop variable into directory scope
unset(_g2_target_type)
unset(_g2_test_targets)
