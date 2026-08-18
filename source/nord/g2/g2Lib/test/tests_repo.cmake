# Test registrations for the repo track. Owned by the repo track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- REPO-5, the ArtifactResolver interface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_artifact_resolver$

add_executable(t0_artifact_resolver t0_artifact_resolver.cpp)
target_link_libraries(t0_artifact_resolver PRIVATE g2Lib)
set_property(TARGET t0_artifact_resolver PROPERTY FOLDER "G2")

add_test(NAME t0_artifact_resolver COMMAND t0_artifact_resolver)
set_tests_properties(t0_artifact_resolver PROPERTIES LABELS "UnitTest")

# ----------------- REPO-7, the skip discipline
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_skip_discipline$
#
# The test builds its own gated subjects through gatedFixture.h. Measured at
# this task's completion: the build carries ZERO gated tests, so a clause
# quantified over "every gated test the build carries" would be vacuously true.
# t0_skip_discipline.cpp states the measurement and the reason in full.

add_executable(t0_skip_discipline t0_skip_discipline.cpp)
target_link_libraries(t0_skip_discipline PRIVATE g2Lib)
set_property(TARGET t0_skip_discipline PROPERTY FOLDER "G2")

add_test(NAME t0_skip_discipline COMMAND t0_skip_discipline)
set_tests_properties(t0_skip_discipline PROPERTIES LABELS "UnitTest")

# ----------------- REPO-8, artifacts.sha256 and golden.timebase
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_manifest_parses$
#
# Both manifests are committed at the ROOT of this repository, so the test is
# given the repository root rather than deriving it from the working directory.
# ctest runs a test from its own binary directory, and a relative path would
# find nothing and be indistinguishable from a manifest that is genuinely
# absent.

# The test also compares each recorded value against the macro that defines it,
# so it needs g2/timebase.h. AN INCLUDE DIRECTORY RATHER THAN g2Lib: the header
# is macros and static inline functions and needs no library, and linking one
# would put a T0 text test behind the whole board's compile.

add_executable(t0_manifest_parses t0_manifest_parses.cpp)
target_compile_definitions(t0_manifest_parses PRIVATE G2_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(t0_manifest_parses PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
set_property(TARGET t0_manifest_parses PROPERTY FOLDER "G2")

add_test(NAME t0_manifest_parses COMMAND t0_manifest_parses)
set_tests_properties(t0_manifest_parses PROPERTIES LABELS "UnitTest")
