cmake_minimum_required(VERSION 3.15)

# The fork's own manifest, and the test that validates it.
#
# artifacts.sha256 lives at the repository root of this default branch because
# it is tooling for operating the fork. The test is given that root explicitly:
# ctest runs a test from its own binary directory, so a relative path would find
# nothing and be indistinguishable from a manifest that is genuinely absent.

add_executable(t0_artifacts_manifest t0_artifacts_manifest.cpp)
target_compile_definitions(t0_artifacts_manifest PRIVATE G2_REPOSITORY_ROOT="${CMAKE_SOURCE_DIR}")
set_property(TARGET t0_artifacts_manifest PROPERTY FOLDER "G2")

add_test(NAME t0_artifacts_manifest COMMAND t0_artifacts_manifest)
set_tests_properties(t0_artifacts_manifest PROPERTIES LABELS "UnitTest")
