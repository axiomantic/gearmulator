cmake_minimum_required(VERSION 3.15)

# THIS FILE HAS ONE OWNER: task BRD-0. See plan section 7.4.2.
#
# It is the REGISTRAR of every test under source/nord/g2/g2Lib/test/. A track
# adds its own add_test(NAME <name> ...) lines to its own per-track list below
# and to nothing else. EVERY TEST IS REGISTERED WITH THE EXACT NAME ITS TASK'S
# Check: LINE PASSES TO -R. Plan section 7.7 lints that correspondence.
#
# ONE CLASS IS EXEMPT: an INFRASTRUCTURE TEST - registered by this file rather
# than by a track, existing to guard the other tests, named by no Check: line.
# One member today, g2_test_executables_build. THE EXEMPTION IS DECLARED AS A
# LABEL, NOT AS THIS SENTENCE: that test carries LABELS "Infrastructure", so the
# class is greppable and a future member joins it by carrying the same label.
#
# CHECK THAT LABEL AGAINST A FRESHLY GENERATED MANIFEST, NEVER A FOUND ONE. On
# 2026-08-12 the manifest here was stamped 03:47 against a 04:21 source and
# cited a backtrace line past the end of this file; the label read ABSENT - a
# fact about the stale manifest, not about this file. After `cmake -S . -B
# build` it lands, in the generated set_tests_properties line and in a ctest
# run's Label Time Summary. Every "verified against the generated manifest"
# claim owes that reconfigure first.
#
# docs/check-targets.txt is deliberately given no line for it; verified
# 2026-08-12, it holds none. planlint's check-targets-mismatch builds `expected`
# from that file's non-comment lines and `actual` from the -R arguments of the
# plan's Check: commands, reporting BOTH directions as ERROR - so a line for an
# infrastructure test would sit in `expected`, never in `actual`, and emit a
# genuine mismatch.
#
# THE SECTION 7.7 LINT MUST EXEMPT THIS CLASS, and what owes this is narrower
# than "the lint". check-targets-mismatch IS committed, in nmg2-tools'
# planlint/checks.py. NOT written is the condition walking the other way - from
# a test REGISTERED in CMake back to a Check: line - the only one that could
# report g2_test_executables_build as matching none. Verified 2026-08-12: that
# name appears nowhere in nmg2-tools. This paragraph is the NOTICE that the
# registration-side condition owes the exemption, never the condition itself.
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
# STALE-ARTIFACT GUARD. It must sit after every add_executable this directory
# declares, and the COMPILE-FAILURE GUARD below must follow it and be LAST.
# THIS block is not the last one in the file; the "no longer last" diagnostic
# further down belongs to that block, never to this one.
#
# Before every link of every executable this directory declares, it deletes that
# executable's output file. A link that then fails leaves nothing behind, so
# ctest cannot run a previous generation's binary; CTest reports an ABSENT
# binary as ***Not Run and counts the test FAILED. PRE_LINK runs only on an
# actual relink, so a no-op rebuild cannot touch a binary ctest is about to run.
#
# ITS MARGINAL VALUE ON THIS TOOLCHAIN IS MEASURED AT ZERO. Measured 2026-08-12,
# macOS 26.5.1 arm64, Apple clang, CMake 4.3.4: a scratch CMake project with NO
# guard loses its binary on a failed link (build exit 2, binary absent); a bare
# `c++ -o out bad.o`, no CMake and no make, deletes a PLAIN TEXT file sitting at
# the output path (driver exit 1, file gone); and the DISCRIMINATING control -
# the driver's own final link line run directly, plain text file at the output
# path - exits 1 and LEAVES it byte for byte. The removal is the clang DRIVER,
# not ld, not make, not CMake. Same day in this repository's build/ tree, with
# t0_mailbox_index's link broken by an undefined symbol from a global
# constructor: `cmake --build` exits 2, this block's rm fires exactly once, the
# binary is ABSENT. Presence and absence produce the same file system here, so a
# deliberate-break demo cannot tell this guard from its own absence.
#
# IT IS KEPT AS DEFENCE. Deleting the output on failure is a driver convention,
# not a guarantee; Linux and Windows are UNVERIFIED, and a toolchain that leaves
# the failed output in place is exactly this block's case. Keep it for that, and
# for the human running a test binary directly.
#
# WHAT IT COSTS IS NOT ONE rm EVERYWHERE. Under Makefiles it is one removal per
# relink. On the setup CLAUDE.md documents as current - Visual Studio 2026 with
# `--config Debug` as the quick compile-check loop - MSVC defaults to
# /INCREMENTAL for Debug, which needs the previous .exe and .ilk, so deleting
# the output should force a FULL link of every test executable on every relink
# of that loop. Documented MSVC behaviour, NOT measured here.
#
# A COMPILE FAILURE BYPASSES IT ENTIRELY: the link never starts, PRE_LINK never
# runs, the previous binary survives. Measured 2026-08-12 with this block
# present: invalid C++ appended to t0_mailbox_index.cpp takes `cmake --build
# build --target t0_mailbox_index` to exit 2, prints ZERO "Discarding any
# previous" lines, and leaves the binary byte-identical by sha256. The
# COMPILE-FAILURE GUARD below closes that route and carries its own numbers.
#
# IT CARRIES NO MECHANISM, and the plan accepts that ONCE, here: section 24.6
# row W3-30's shape, second acceptance clause. The first is refused MECHANICALLY
# - the only available check is the coverage recipe below, which reads
# build.make, and ONLY the Makefile generator writes build.make. ci.yml's matrix
# targets linux-x86-64 and windows-x86-64 (macos-arm64 commented out), where the
# generator is Visual Studio or Ninja, so registering that count would turn the
# check RED on Windows by construction.
#
# THE INCIDENT THIS BLOCK USED TO CITE WAS NOT ONE. a4278548 (2026-08-08) added
# board.cpp, board.h and t0_board_surface.cpp and ZERO CMake files, so until
# f8791f9a (2026-08-10) added sources_board.cmake and tests_board.cmake there
# was no target and nothing for ctest to run. That false completion lived in the
# task ledger, not in ctest output.
#
# COVERAGE IS A DIAGNOSTIC A READER MAY RUN, NEVER A COUNT THIS BLOCK CLAIMS.
# Match the PRE_LINK line, which carries the output PATH, from the build
# directory mirroring THIS source directory:
#
#   find CMakeFiles -maxdepth 2 -name build.make \
#     -exec grep -lE '^[[:space:]]*cd .* && .*cmake -E rm -f /' {} \; | wc -l
#
# TWO NAIVE FORMS OVER-REPORT, AND BOTH OVER-REPORT IN THIS TREE RIGHT NOW.
# Measured 2026-08-12 in build/source/nord/g2/g2Lib/test: the recipe gives 44;
# the bare string `cmake -E rm -f` over the same files gives 48, because every
# build.make carries the boilerplate `RM = <cmake> -E rm -f` and orphan .dir
# directories CMake leaves for removed targets match it; the recipe run
# RECURSIVELY from the build directory gives 88, because SCH-3's t0_clock_guard
# configures nested CMake projects under t0_clock_guard_work, swept in once that
# test has RUN - exactly when a reader reaches for this, and present today.
# `find CMakeFiles` excludes the sub-build; -maxdepth excludes nothing here. A
# FRESH tree has neither orphans nor sub-build and all three agree, which is
# what makes both traps quiet.
#
# THE CMake FLOOR THIS BLOCK NEEDS IS 3.26, above the 3.15 the file declares,
# and cmake_minimum_required is deliberately NOT raised, because raising a
# fork's floor is a divergence. From the CMake 4.3.4 docs installed here:
# add_custom_command's TARGET form gained target-dependent generator expressions
# in 3.21, which is what $<TARGET_FILE:...> in COMMAND is; COMMENT accepts them
# only from 3.26; and `cmake -E rm` arrived in 3.17, the older spelling being
# `cmake -E remove`, deprecated in that same release. A `remove` shim is the
# only thing that would buy 3.15 and 3.16, and none is kept, because below 3.26
# the effect is untested here regardless. THAT FLOOR IS RECORDED NOWHERE BUT
# HERE: .github and docs hold no occurrence of 3.26 or 3.21, and REPO-6's ci.yml
# floor step is written for "3.20 or newer". Recording it plan-side is BRD-0's
# work and is NOT DONE.
# ---------------------------------------------------------------------------

get_property(_g2_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
get_property(_g2_registered_tests DIRECTORY PROPERTY TESTS)	# read BEFORE the fixture's own add_test below

set(_g2_test_executables "")

foreach(_g2_target IN LISTS _g2_test_targets)
	get_target_property(_g2_target_type ${_g2_target} TYPE)

	if(_g2_target_type STREQUAL "EXECUTABLE")
		list(APPEND _g2_test_executables ${_g2_target})

		add_custom_command(TARGET ${_g2_target} PRE_LINK
			COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE:${_g2_target}>
			COMMENT "Discarding any previous $<TARGET_FILE_NAME:${_g2_target}> so a failed link cannot be tested")
	endif()
endforeach()

# ---------------------------------------------------------------------------
# COMPILE-FAILURE GUARD. Keep this block LAST in this file.
#
# It declares one CTest fixture whose setup test BUILDS every executable this
# directory declares, and makes every test this directory registers require that
# fixture. A compile failure takes the fixture red, dependents report ***Not
# Run, ctest exits non-zero. THERE IS NO STAMP, deliberately: the fixture passes
# when and only when `cmake --build` exits 0 in this run, and a stamp written by
# a successful build outlives the NEXT build's failure - the shape of the stale
# binary this file exists to prevent.
#
# WHAT IT BUYS, MEASURED 2026-08-12, CMake and CTest 4.3.4, macOS 26.5.1 arm64,
# Apple clang, Makefile generator, in this repository's build/ tree, all with
# `-R ^t0_mailbox_index$ --no-tests=error`:
#   - clean tree: exits 0, Passed. It does not fire on healthy code.
#   - invalid C++ appended to t0_mailbox_index.cpp and NO build run afterwards:
#     exits 8, t0_mailbox_index ***Not Run, "0% tests passed, 2 tests failed out
#     of 2". With this block deleted from this file and the tree reconfigured,
#     the identical break exits 0 and prints "100% tests passed, 0 tests failed
#     out of 1" - green over the previous generation's binary. That pair is the
#     whole case for this block.
#   - invalid C++ in t0_mailbox_count.cpp INSTEAD, so the target under test is
#     not the one that failed: exits 8. The parallel-abort case is covered.
#
# WHAT IT COSTS: one build per ctest invocation, 1.62 s with nothing out of
# date. A ctest run now WRITES to the build tree and contends with a concurrent
# build. AND IT CAN REWRITE THE FILE THAT DEFINES THE RUN, DURING THE RUN: the
# fixture's `cmake --build` goes through cmake_check_build_system, so a
# CMakeLists edit since the last configure is picked up FROM INSIDE the ctest
# run and CTestTestfile.cmake is regenerated while CTest executes the manifest
# it loaded at startup. Measured 2026-08-12 outside this repository so the tree
# could be driven precisely: a test added after configure took the manifest from
# 2 add_test lines to 3 and changed its sha256, during a run reporting "100%
# tests passed, 0 tests failed out of 2" that never ran the third. The verdict
# describes the manifest CTest loaded, not the one on disk when it finished.
#
# ITS BLAST RADIUS IS THE WHOLE DIRECTORY, deliberately: one broken source
# blocks every test here. One fixture per target was rejected because it pays a
# no-op build - measured 0.58 s for a single target - once per TEST rather than
# once per RUN, and because a binary left stale by a failed sibling must go loud.
#
# IT COVERS THE LINK ROUTE TOO, so the block above is subsumed for anything
# going through ctest and is kept for what does not. Measured the same day with
# an undefined symbol from a global constructor: build exits 2, PRE_LINK fires
# once, the binary is ABSENT, ctest exits 8 - the fixture goes red on the build's
# exit status, so its verdict does not depend on the stale binary surviving. NOT
# subsumed: a human running a test binary directly.
#
# COVERAGE AS GENERATED TODAY, re-measured 2026-08-12 after a reconfigure: of
# the 46 add_test lines in this directory's CTestTestfile.cmake, 45 carry
# FIXTURES_REQUIRED, the one that does not being the fixture's own setup test.
# 44 of those 45 are executables; t0_timebase_header is a `cmake -P` script test
# compiling its own source at test time, with no binary to go stale.
#
# CTEST FLAGS CAN DEFEAT THIS GUARD. CTest carries a test-selection family and a
# fixture-suppression family - -E, -FS, -FA - and any fixture-based guard is
# defeasible by them: drop the setup test out of a run while its dependents
# still execute, no build happens, a stale binary passes. Suppressing a fixture
# is what the -F flags are FOR and no CMakeLists can disable them. None of it
# unblocks ONE test; it restores the unguarded behaviour for the WHOLE
# selection, silently. One measurement, an existence proof and NOT a rule:
# 2026-08-12 in this tree, `ctest -R ^t0_mailbox_index$ --no-tests=error -FA
# g2_test_executables_built` over a compile-broken source with no build
# afterwards exits 0, prints "100% tests passed, 0 tests failed out of 1", and
# runs the previous generation's binary, byte-identical by sha256.
#
# WHICH COMBINATIONS ESCAPE IS NOT STATED HERE AND MUST NOT BE ADDED. Four
# rounds tabulated it; each table was refuted by the first cell the next round
# tried. A READER WHO NEEDS TO KNOW MUST MEASURE THEIR OWN INVOCATION: break a
# source, run their exact ctest command line with no build after it, read the
# exit status. That is the only claim here that has survived every round. The
# exit that keeps the check is to fix the broken source, or comment it out of
# its tests_*.cmake list.
#
# ITS OWN SILENT-FAILURE MODES ARE MADE LOUD BELOW, because a guard whose
# absence looks exactly like its presence fails like the defect it was built to
# catch. Each is a configure-time FATAL_ERROR, not an AUTHOR_WARNING (-Wno-dev
# silences the latter), and each means tests here run unguarded right now. Each
# was WATCHED TO FIRE on 2026-08-12 in a replica built from the code below.
#
#   1. BLOCK NOT LAST. An add_executable after this block is in no aggregate, so
#      a compile failure in it leaves the fixture GREEN over a stale binary; an
#      add_test after it carries no FIXTURES_REQUIRED at all.
#   2. TESTS BUT NO EXECUTABLES. The if() below is false, no fixture is created,
#      configure succeeds, every test here runs unguarded.
#   3. TESTS IN A SUBDIRECTORY. The DIRECTORY property TESTS does not descend.
#
# NONE OF THE THREE IS REACHABLE IN THIS DIRECTORY TODAY, verified rather than
# assumed: nothing declares an executable or registers a test below this block
# except the block's own add_test; 44 executables are declared, so mode 2's
# branch cannot be taken; and neither this file nor any tests_*.cmake calls
# add_subdirectory. Configure exits 0 here - which is also what all three print
# when they have nothing to say, and why each was watched to fire where it could.
#
# ALL THREE RUN DEFERRED, which is the whole fix for the defect this block once
# reproduced in ITSELF. Measured against the immediate form, cmake_language
# (DEFER) removed and everything else identical: an add_executable plus add_test
# placed AFTER this block escaped all three FATAL_ERRORs and reproduced the
# audited defect exactly - configure exit 0 with no diagnostic, 1 of 3 tests
# carrying FIXTURES_REQUIRED, a compile break taking the build to exit 2 with
# the binary byte-identical by sha256, ctest exit 0 printing "100% tests passed,
# 0 tests failed out of 3". An add_subdirectory there instead escaped
# identically, zero FIXTURES_REQUIRED in the subdirectory's manifest, same
# verdict: the walk ran BEFORE the block, and the deferred last-check re-read
# BUILDSYSTEM_TARGETS and TESTS, NEITHER OF WHICH DESCENDS.
#
# THE DEFER IS ARMED UNCONDITIONALLY, outside the if(), and the guarded lists
# are recorded unconditionally too. Both were measured against the alternative,
# in the one directory shape that takes NEITHER branch below - EXECUTABLES
# DECLARED, NO TEST REGISTERED AT GUARD TIME. With the arming inside, that shape
# scheduled no deferred call and left a test added below it unguarded, mode 1
# unable to report it: configure exit 0, zero FIXTURES_REQUIRED, late test
# present. With the lists recorded only inside, the deferred check read them
# back empty and named an executable declared ABOVE the block as late - a false
# diagnostic in that same shape, which is NOT mode 2: mode 2 is tests with no
# executables, this is executables with no test.
#
# TWO CHECKS CARRY A DECLARABLE OPT-OUT, because a legitimate layout can trip
# them and the only other exit would be editing this file while the directory is
# blocked. Each suppresses exactly one check: measured, with both declared, mode
# 1 still fires.
#   - G2_GUARD_ALLOW_SCRIPT_ONLY_TESTS, set before this block, declares a
#     script-only directory deliberate - a `cmake -P` test has no executable and
#     needs no fixture, and THIS DIRECTORY ALREADY CONTAINS ONE,
#     t0_timebase_header, guarded today only by the accident that 44 executables
#     sit beside it. Suppresses mode 2; mode 3 still fires beside it.
#   - G2_GUARD_SELF_GUARDED, a directory property a SUBDIRECTORY sets on itself,
#     declares that subtree to install its own equivalent fixture; the walk then
#     skips it whole, on the reading that a subdirectory carrying this block
#     carries this block's recursion. Suppresses mode 3 only.
# THEY DIFFER IN REACH, and not decoratively. Measured:
# `cmake -DG2_GUARD_ALLOW_SCRIPT_ONLY_TESTS=TRUE` DOES suppress mode 2 - it is
# an ordinary variable, settable by anyone configuring the tree.
# `cmake -DG2_GUARD_SELF_GUARDED=TRUE` does NOT suppress mode 3, because that
# one is read as a DIRECTORY PROPERTY and -D cannot set one. "Declared at the
# site that knows the fact" is the intent of both, not a property of either.
#
# THE FLOOR THESE CHECKS NEED IS 3.19, for cmake_language(DEFER) - below the
# 3.26 established above, but ABOVE the 3.15 this file DECLARES on line 1, and
# below 3.19 DEFER is a HARD configure failure on an unknown argument, not a
# degraded effect. ci.yml carries a step NAMED "Require CMake and CTest 3.20 or
# newer" that is weaker than its name, in the generous direction: it prints
# `ctest --version` but compares only `cmake --version`, leaving the CTest half
# unenforced. The CMake half does cover 3.19.
#
# UNVERIFIED, AND THE UNTESTED GENERATOR IS THE PRIMARY ONE. Every runtime
# number above was taken under a Makefile generator on macOS, while CLAUDE.md
# documents Visual Studio 2026 as current. The CONFIGURE-time checks are better
# off: cmake.yml runs on every push and pull request across ubuntu-latest,
# macos-14 and windows-2022 over the default and Ninja generators, and
# nightly.yml configures and builds those same three runners on a daily
# `0 0 * * *` cron. Neither runs ctest - grepped, ci.yml is the only one that
# does, and it is workflow_dispatch-only - and neither builds this aggregate,
# since add_custom_target here carries no ALL.
#
# THE AGGREGATE SUBSTITUTION IS MEASURABLE, NOT INFERRED, and worth measuring,
# because a setup test that built less than it claims is indistinguishable from
# a correct pass. `build/CMakeFiles/Makefile2` carries exactly 44
# `g2_test_executables_all.dir/all:` dependency lines, IDENTICAL to the 44
# executable tests in this directory's manifest - compared element by element on
# 2026-08-12, difference empty both ways. Re-run it; do not trust this line.
#
# THE MULTI-CONFIG HAZARD IS NOT THE ONE THIS BLOCK USED TO NAME. The claim that
# the setup test could build a config other than the dependent tests' is
# FALSIFIED, measured 2026-08-12 under Ninja Multi-Config: CMake emits add_test
# PER CONFIG inside `if(CTEST_CONFIGURATION_TYPE MATCHES ...)` branches, the
# setup test gets the same branch structure as every dependent, and both take
# `--config` from the SAME branch and cannot diverge. The real hazard is the
# else() arm, which emits NOT_AVAILABLE for setup and dependents alike: there,
# ctest with no -C exits 8 with every test ***Not Run and `ctest -C Debug` exits
# 0. So ci.yml invoking ctest with no -C on a Windows tree configured with the
# default multi-config generator fails LOUD - still a defect, still not fixed
# here, but a red job rather than a false green.
# ---------------------------------------------------------------------------

# Walks SUBDIRECTORIES recursively. A subtree declaring itself self-guarded is
# skipped whole: the declaration asserts it installs an equivalent fixture, and
# a subdirectory carrying this block carries this recursion with it.
function(_g2_collect_subdirectory_tests _g2_dir _g2_out)
	set(_g2_found "")

	get_property(_g2_subs DIRECTORY "${_g2_dir}" PROPERTY SUBDIRECTORIES)

	foreach(_g2_sub IN LISTS _g2_subs)
		get_property(_g2_self_guarded DIRECTORY "${_g2_sub}" PROPERTY G2_GUARD_SELF_GUARDED)

		if(_g2_self_guarded)
			continue()
		endif()

		get_property(_g2_subtests DIRECTORY "${_g2_sub}" PROPERTY TESTS)

		foreach(_g2_name IN LISTS _g2_subtests)
			list(APPEND _g2_found "${_g2_name} (in ${_g2_sub})")
		endforeach()

		_g2_collect_subdirectory_tests("${_g2_sub}" _g2_deeper)	# a subdirectory of a subdirectory is the same gap
		list(APPEND _g2_found ${_g2_deeper})
	endforeach()

	set(${_g2_out} "${_g2_found}" PARENT_SCOPE)
endfunction()

# Runs at the END of this directory, after everything below it: the only vantage
# point from which "was this block last?" and "did a subdirectory appear?" are
# both answerable. It reads its inputs back off directory properties rather than
# off variables because the unset() calls at the foot of this file have already
# run by then.
function(_g2_guard_assert_complete)
	get_property(_g2_final_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
	get_property(_g2_final_tests DIRECTORY PROPERTY TESTS)
	get_property(_g2_guarded_executables DIRECTORY PROPERTY _g2_guard_executables)
	get_property(_g2_guarded_tests DIRECTORY PROPERTY _g2_guard_tests)

	# Check 3, run from here rather than inline: BUILDSYSTEM_TARGETS and TESTS do
	# not descend, so an add_subdirectory() below this block is invisible to the
	# late-declaration check and would otherwise escape every diagnostic.
	_g2_collect_subdirectory_tests("${CMAKE_CURRENT_SOURCE_DIR}" _g2_subdirectory_tests)

	if(_g2_subdirectory_tests)
		message(FATAL_ERROR
			"COMPILE-FAILURE GUARD: tests are registered in a SUBDIRECTORY of this one.\n"
			"  The guard reads the DIRECTORY property TESTS, which does not descend, so these\n"
			"  tests would get no fixture and would run against whatever binary is on disk:\n"
			"    ${_g2_subdirectory_tests}\n"
			"  Register them in this directory, extend this guard to cover that subdirectory,\n"
			"  or, if that subdirectory installs its own equivalent fixture, declare it with\n"
			"    set_property(DIRECTORY PROPERTY G2_GUARD_SELF_GUARDED TRUE)\n"
			"  in that subdirectory's own CMakeLists.txt.")
	endif()

	set(_g2_final_executables "")

	foreach(_g2_final_target IN LISTS _g2_final_targets)
		get_target_property(_g2_final_type ${_g2_final_target} TYPE)

		if(_g2_final_type STREQUAL "EXECUTABLE")
			list(APPEND _g2_final_executables ${_g2_final_target})
		endif()
	endforeach()

	set(_g2_late_executables ${_g2_final_executables})

	if(_g2_guarded_executables)
		list(REMOVE_ITEM _g2_late_executables ${_g2_guarded_executables})
	endif()

	set(_g2_late_tests ${_g2_final_tests})
	list(REMOVE_ITEM _g2_late_tests g2_test_executables_build)	# this block's own registration, not a late one

	if(_g2_guarded_tests)
		list(REMOVE_ITEM _g2_late_tests ${_g2_guarded_tests})
	endif()

	if(_g2_late_executables OR _g2_late_tests)
		message(FATAL_ERROR
			"COMPILE-FAILURE GUARD: this block is no longer last in this file.\n"
			"  Executables declared after it, absent from g2_test_executables_all, so a\n"
			"  failed compile of one leaves the fixture green over a stale binary:\n"
			"    ${_g2_late_executables}\n"
			"  Tests registered after it, carrying no FIXTURES_REQUIRED at all:\n"
			"    ${_g2_late_tests}\n"
			"  Move those declarations above this block, or move this block below them.")
	endif()
endfunction()

# Recorded and armed UNCONDITIONALLY. Inside the if() below, neither survives a
# directory that declares executables but registers no test at guard time - and
# the checks that exist to report that directory are the ones that go missing.
set_property(DIRECTORY PROPERTY _g2_guard_executables "${_g2_test_executables}")
set_property(DIRECTORY PROPERTY _g2_guard_tests "${_g2_registered_tests}")
cmake_language(DEFER CALL _g2_guard_assert_complete)

if(_g2_test_executables AND _g2_registered_tests)
	# ONE aggregate target, not the whole executable list passed to --target.
	# Under the Makefile generator each --target argument is its own recursive
	# make and its own walk of the graph: measured 2026-08-12 over the 44
	# executables this directory declares, with nothing out of date, the list
	# form cost 21.75 s and the aggregate form 1.69 s.
	add_custom_target(g2_test_executables_all)
	add_dependencies(g2_test_executables_all ${_g2_test_executables})

	add_test(NAME g2_test_executables_build
		COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target g2_test_executables_all)

	# LABELS declares the infrastructure-test exemption the header names, in a
	# form a lint can read. FIXTURES_SETUP is what makes it the fixture.
	set_tests_properties(g2_test_executables_build PROPERTIES
		FIXTURES_SETUP g2_test_executables_built
		LABELS "Infrastructure")

	# APPEND, not PROPERTIES. `set_tests_properties(... PROPERTIES ...)` SETS, so
	# it silently deletes any fixture a track already required on its own test.
	# Measured 2026-08-12 both ways in a replica, on a test carrying
	# FIXTURES_REQUIRED track_data: this APPEND form generates
	# "track_data;g2_test_executables_built"; the PROPERTIES form generates
	# "g2_test_executables_built" alone, configure exit 0, no diagnostic.
	# Unreachable today - tracks set only LABELS "UnitTest" - which is precisely
	# the reachability of the three modes above.
	set_property(TEST ${_g2_registered_tests} APPEND PROPERTY FIXTURES_REQUIRED g2_test_executables_built)
elseif(_g2_registered_tests AND NOT G2_GUARD_ALLOW_SCRIPT_ONLY_TESTS)
	message(FATAL_ERROR
		"COMPILE-FAILURE GUARD: this directory registers tests but declares no executable,\n"
		"  so no fixture is created and every test here runs unguarded:\n"
		"    ${_g2_registered_tests}\n"
		"  A directory of `cmake -P` script tests has no binary to go stale and needs no\n"
		"  build fixture. If that is the case here, declare it before this block with\n"
		"    set(G2_GUARD_ALLOW_SCRIPT_ONLY_TESTS TRUE)\n"
		"  rather than editing this guard.")
endif()

unset(_g2_target)	# CMP0124 is unset (so it behaves as OLD) and foreach() leaks the loop variable into directory scope
unset(_g2_target_type)
unset(_g2_test_targets)
unset(_g2_test_executables)
unset(_g2_registered_tests)
