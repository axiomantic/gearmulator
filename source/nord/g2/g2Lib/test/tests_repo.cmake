# Test registrations for the repo track. Owned by the repo track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. The name is the exact string passed to -R.
# Edit no other CMake file in this tree.

# ----------------- the ArtifactResolver interface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_artifact_resolver$

add_executable(t0_artifact_resolver t0_artifact_resolver.cpp)
target_link_libraries(t0_artifact_resolver PRIVATE g2Lib)
set_property(TARGET t0_artifact_resolver PROPERTY FOLDER "G2")

add_test(NAME t0_artifact_resolver COMMAND t0_artifact_resolver)
set_tests_properties(t0_artifact_resolver PROPERTIES LABELS "UnitTest")

# ----------------- golden.timebase
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_manifest_parses$
#
# golden.timebase is committed at the ROOT of this repository, so the test is
# given the repository root rather than deriving it from the working directory.
# ctest runs a test from its own binary directory, and a relative path would
# find nothing and be indistinguishable from a manifest that is genuinely
# absent.
#
# artifacts.sha256 is NOT here and this test no longer reads it. It is fork
# tooling and lives on the default branch, validated there by
# source/nord/g2Tooling/t0_artifacts_manifest. golden.timebase stays with
# g2/timebase.h because every value in it names a macro in that header and is
# compared against it below -- separating the two is what left this test unable
# to pass on any branch between 2026-09-07 and 2026-09-10.

# The test also compares each recorded value against the macro that defines it,
# so it needs g2/timebase.h. An include directory rather than g2Lib: the header
# is macros and static inline functions and needs no library, and linking one
# would put a T0 text test behind the whole board's compile.

add_executable(t0_manifest_parses t0_manifest_parses.cpp)
target_compile_definitions(t0_manifest_parses PRIVATE G2_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(t0_manifest_parses PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
set_property(TARGET t0_manifest_parses PROPERTY FOLDER "G2")

add_test(NAME t0_manifest_parses COMMAND t0_manifest_parses)
set_tests_properties(t0_manifest_parses PROPERTIES LABELS "UnitTest")
