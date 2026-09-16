#!/usr/bin/env bash
# Run the registered test suite, minus the labels CI_TEST_EXCLUDE_LABELS names.
#
# The suite is selected by what it is NOT, so a test added anywhere in this tree
# is run without anyone editing a list. Removing a label from
# CI_TEST_EXCLUDE_LABELS is how to turn its tests on.
#
# --no-tests=error is why the CMake floor in require-cmake-version.sh is 3.20:
# without it a filtered run that selects nothing exits 0, and a pass over no code
# at all is indistinguishable from a pass.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

ci_log "ctest in ${CI_BUILD_DIR}, excluding labels ${CI_TEST_EXCLUDE_LABELS}"
exec ctest --test-dir "${CI_BUILD_DIR}" \
	--no-tests=error \
	-LE "${CI_TEST_EXCLUDE_LABELS}" \
	--output-on-failure
