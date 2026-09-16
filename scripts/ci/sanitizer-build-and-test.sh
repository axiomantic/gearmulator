#!/usr/bin/env bash
# Configure, build and test under the address and undefined sanitizers.
#
# This is its own script rather than a CI_CMAKE_ARGS call because the flag values
# contain spaces, and CI_CMAKE_ARGS is word-split on its way from a workflow
# matrix into an argument list.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

san_dir=${CI_SANITIZER_BUILD_DIR:-${CI_REPO_ROOT}/build-asan}
san_flags="-fsanitize=address,undefined -fno-omit-frame-pointer"

ci_log "configure ${san_dir} with the address and undefined sanitizers"
cmake -S "${CI_REPO_ROOT}" -B "${san_dir}" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_C_FLAGS="${san_flags}" \
	-DCMAKE_CXX_FLAGS="${san_flags}"

ci_log "build ${san_dir} (parallel ${CI_PARALLEL})"
cmake --build "${san_dir}" --parallel "${CI_PARALLEL}"

CI_BUILD_DIR=${san_dir} exec "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/test.sh"
