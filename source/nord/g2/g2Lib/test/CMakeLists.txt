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
# THE EXEMPTION IS DECLARED AS A LABEL, NOT AS THIS SENTENCE: such a test
# carries LABELS "Infrastructure", so the class is greppable and a future
# member joins it by carrying the same label.
#
# CHECK THAT LABEL AGAINST A FRESHLY GENERATED MANIFEST, NEVER A FOUND ONE. A
# stale manifest reports a fact about itself, not about this file. After
# `cmake -S . -B build` the label lands, in the generated set_tests_properties
# line and in a ctest run's Label Time Summary. Every "verified against the
# generated manifest" claim owes that reconfigure first.
#
# docs/check-targets.txt is deliberately given no line for an infrastructure
# test. planlint's check-targets-mismatch builds `expected` from that file's
# non-comment lines and `actual` from the -R arguments of the plan's Check:
# commands, reporting BOTH directions as ERROR - so a line for an
# infrastructure test would sit in `expected`, never in `actual`, and emit a
# genuine mismatch.
#
# THE SECTION 7.7 LINT MUST EXEMPT THIS CLASS, and what owes this is narrower
# than "the lint". check-targets-mismatch IS committed, in nmg2-tools'
# planlint/checks.py. NOT written is the condition walking the other way - from
# a test REGISTERED in CMake back to a Check: line - the only one that could
# report an infrastructure test as matching none. This paragraph is the NOTICE
# that the registration-side condition owes the exemption, never the condition
# itself.

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
# ITS MARGINAL VALUE ON THIS TOOLCHAIN IS SMALL. On a clang driver the removal
# of a failed link's output is done by the DRIVER, not by ld, not by make and
# not by CMake, so presence and absence of this block produce the same file
# system here and a deliberate-break demo cannot tell this guard from its own
# absence.
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
# runs, the previous binary survives. The COMPILE-FAILURE GUARD below closes
# that route.
#
# IT CARRIES NO MECHANISM, and the plan accepts that ONCE, here: section 24.6
# row W3-30's shape, second acceptance clause. The first is refused MECHANICALLY
# - the only available check is the coverage recipe below, which reads
# build.make, and ONLY the Makefile generator writes build.make. ci.yml's matrix
# targets linux-x86-64 and windows-x86-64 (macos-arm64 commented out), where the
# generator is Visual Studio or Ninja, so registering that count would turn the
# check RED on Windows by construction.
#
# COVERAGE IS A DIAGNOSTIC A READER MAY RUN, NEVER A COUNT THIS BLOCK CLAIMS.
# Match the PRE_LINK line, which carries the output PATH, from the build
# directory mirroring THIS source directory:
#
#   find CMakeFiles -maxdepth 2 -name build.make \
#     -exec grep -lE '^[[:space:]]*cd .* && .*cmake -E rm -f /' {} \; | wc -l
#
# TWO NAIVE FORMS OVER-REPORT. The bare string `cmake -E rm -f` over the same
# files over-reports, because every build.make carries the boilerplate
# `RM = <cmake> -E rm -f` and orphan .dir directories CMake leaves for removed
# targets match it. The recipe run RECURSIVELY from the build directory
# over-reports too, because t0_clock_guard configures nested CMake projects
# under t0_clock_guard_work, swept in once that test has RUN - exactly when a
# reader reaches for this. `find CMakeFiles` excludes the sub-build; -maxdepth
# excludes nothing here. A FRESH tree has neither orphans nor sub-build and all
# three agree, which is what makes both traps quiet.
#
# THE CMake FLOOR THIS BLOCK NEEDS IS 3.26, above the 3.15 the file declares,
# and cmake_minimum_required is deliberately NOT raised, because raising a
# fork's floor is a divergence. add_custom_command's TARGET form gained
# target-dependent generator expressions in 3.21, which is what
# $<TARGET_FILE:...> in COMMAND is; COMMENT accepts them only from 3.26; and
# `cmake -E rm` arrived in 3.17, the older spelling being `cmake -E remove`,
# deprecated in that same release. A `remove` shim is the only thing that would
# buy 3.15 and 3.16, and none is kept, because below 3.26 the effect is
# untested here regardless. THAT FLOOR IS RECORDED NOWHERE BUT HERE, and
# REPO-6's ci.yml floor step is written for "3.20 or newer". Recording it
# plan-side is BRD-0's work and is NOT DONE.
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
# WHAT IT BUYS: a compile break with no build run afterwards takes ctest
# non-zero and reports the dependent ***Not Run. Without this block the
# identical break passes green over the previous generation's binary. That pair
# is the whole case for this block. A break in a sibling source, so that the
# target under test is not the one that failed, is caught the same way.
#
# WHAT IT COSTS: one build per ctest invocation. A ctest run now WRITES to the
# build tree and contends with a concurrent build. AND IT CAN REWRITE THE FILE
# THAT DEFINES THE RUN, DURING THE RUN: the fixture's `cmake --build` goes
# through cmake_check_build_system, so a CMakeLists edit since the last
# configure is picked up FROM INSIDE the ctest run and CTestTestfile.cmake is
# regenerated while CTest executes the manifest it loaded at startup. The
# verdict describes the manifest CTest loaded, not the one on disk when it
# finished.
#
# ITS BLAST RADIUS IS THE WHOLE DIRECTORY, deliberately: one broken source
# blocks every test here. One fixture per target was rejected because it pays a
# no-op build once per TEST rather than once per RUN, and because a binary left
# stale by a failed sibling must go loud.
#
# IT COVERS THE LINK ROUTE TOO, so the block above is subsumed for anything
# going through ctest and is kept for what does not. The fixture goes red on the
# build's exit status, so its verdict does not depend on the stale binary
# surviving. NOT subsumed: a human running a test binary directly.
#
# A `cmake -P` script test that compiles its own source at test time has no
# binary to go stale, and so needs no fixture of its own.
#
# CTEST FLAGS CAN DEFEAT THIS GUARD. CTest carries a test-selection family and a
# fixture-suppression family - -E, -FS, -FA - and any fixture-based guard is
# defeasible by them: drop the setup test out of a run while its dependents
# still execute, no build happens, a stale binary passes. Suppressing a fixture
# is what the -F flags are FOR and no CMakeLists can disable them. None of it
# unblocks ONE test; it restores the unguarded behaviour for the WHOLE
# selection, silently.
#
# WHICH COMBINATIONS ESCAPE IS NOT STATED HERE AND MUST NOT BE ADDED. Every
# attempt to tabulate it was refuted by the first cell the next attempt tried.
# A READER WHO NEEDS TO KNOW MUST MEASURE THEIR OWN INVOCATION: break a source,
# run their exact ctest command line with no build after it, read the exit
# status. The exit that keeps the check is to fix the broken source, or comment
# it out of its tests_*.cmake list.
#
# ITS OWN SILENT-FAILURE MODES ARE MADE LOUD BELOW, because a guard whose
# absence looks exactly like its presence fails like the defect it was built to
# catch. Each is a configure-time FATAL_ERROR, not an AUTHOR_WARNING (-Wno-dev
# silences the latter), and each means tests here run unguarded right now.
#
#   1. BLOCK NOT LAST. An add_executable after this block is in no aggregate, so
#      a compile failure in it leaves the fixture GREEN over a stale binary; an
#      add_test after it carries no FIXTURES_REQUIRED at all.
#   2. TESTS BUT NO EXECUTABLES. The if() below is false, no fixture is created,
#      configure succeeds, every test here runs unguarded.
#   3. TESTS IN A SUBDIRECTORY. The DIRECTORY property TESTS does not descend.
#
# ALL THREE RUN DEFERRED, which is the whole fix for the defect this block is
# built to catch. Under the immediate form an add_executable plus add_test
# placed AFTER this block escapes all three FATAL_ERRORs and reproduces the
# audited defect exactly. An add_subdirectory there instead escapes
# identically: the walk runs BEFORE the block, and only a deferred last-check
# re-reads BUILDSYSTEM_TARGETS and TESTS, NEITHER OF WHICH DESCENDS.
#
# THE DEFER IS ARMED UNCONDITIONALLY, outside the if(), and the guarded lists
# are recorded unconditionally too, for the directory shape that takes NEITHER
# branch below - EXECUTABLES DECLARED, NO TEST REGISTERED AT GUARD TIME. With
# the arming inside, that shape schedules no deferred call and leaves a test
# added below it unguarded, mode 1 unable to report it. With the lists recorded
# only inside, the deferred check reads them back empty and names an executable
# declared ABOVE the block as late - a false diagnostic in that same shape,
# which is NOT mode 2: mode 2 is tests with no executables, this is executables
# with no test.
#
# SOME CHECKS CARRY A DECLARABLE OPT-OUT, because a legitimate layout can trip
# them and the only other exit would be editing this file while the directory is
# blocked. Each suppresses exactly one check.
#   - G2_GUARD_ALLOW_SCRIPT_ONLY_TESTS, set before this block, declares a
#     script-only directory deliberate - a `cmake -P` test has no executable and
#     needs no fixture. Suppresses mode 2; mode 3 still fires beside it.
#   - G2_GUARD_SELF_GUARDED, a directory property a SUBDIRECTORY sets on itself,
#     declares that subtree to install its own equivalent fixture; the walk then
#     skips it whole, on the reading that a subdirectory carrying this block
#     carries this block's recursion. Suppresses mode 3 only.
# THEY DIFFER IN REACH, and not decoratively.
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
# UNVERIFIED, AND THE UNTESTED GENERATOR IS THE PRIMARY ONE. The runtime
# behaviour above was observed under a Makefile generator on macOS, while
# CLAUDE.md documents Visual Studio 2026 as current. The CONFIGURE-time checks
# are better off: cmake.yml runs on every push and pull request across
# ubuntu-latest, macos-14 and windows-2022 over the default and Ninja
# generators, and nightly.yml configures and builds those same runners on a
# daily `0 0 * * *` cron. Neither runs ctest, and neither builds this
# aggregate, since add_custom_target here carries no ALL.
#
# THE AGGREGATE SUBSTITUTION IS MEASURABLE, NOT INFERRED, and worth measuring,
# because a setup test that built less than it claims is indistinguishable from
# a correct pass. Compare `build/CMakeFiles/Makefile2`'s
# `g2_test_executables_all.dir/all:` dependency lines against the executable
# tests in this directory's manifest, element by element. Measure it; do not
# trust a written figure.
#
# THE MULTI-CONFIG HAZARD IS NOT THE OBVIOUS ONE. The setup test cannot build a
# config other than the dependent tests': CMake emits add_test PER CONFIG
# inside `if(CTEST_CONFIGURATION_TYPE MATCHES ...)` branches, the setup test
# gets the same branch structure as every dependent, and both take `--config`
# from the SAME branch and cannot diverge. The real hazard is the else() arm,
# which emits NOT_AVAILABLE for setup and dependents alike: there, ctest with
# no -C exits 8 with every test ***Not Run and `ctest -C Debug` exits 0. So
# ci.yml invoking ctest with no -C on a Windows tree configured with the
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
	# make and its own walk of the graph, so the list form costs an order of
	# magnitude more than the aggregate form even with nothing out of date.
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
	# The APPEND form preserves both; the PROPERTIES form drops the track's own
	# fixture with configure exit 0 and no diagnostic.
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
