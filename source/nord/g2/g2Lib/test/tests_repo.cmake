# Test registrations for the repo track. Owned by the repo track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.
#
# Created empty by task BRD-0.

# ----------------- REPO-5, the ArtifactResolver interface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_artifact_resolver$

add_executable(t0_artifact_resolver t0_artifact_resolver.cpp)
target_link_libraries(t0_artifact_resolver PRIVATE g2Lib)
set_property(TARGET t0_artifact_resolver PROPERTY FOLDER "G2")

add_test(NAME t0_artifact_resolver COMMAND t0_artifact_resolver)
set_tests_properties(t0_artifact_resolver PROPERTIES LABELS "UnitTest")
