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
