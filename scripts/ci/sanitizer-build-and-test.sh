#!/usr/bin/env bash
# Configure, build and test under the address and undefined sanitizers.
#
# The sanitizer flags are set here rather than passed through CI_CMAKE_ARGS
# because their values contain spaces, and CI_CMAKE_ARGS is word-split on its
# way from a workflow matrix into an argument list. CI_CMAKE_ARGS is still read,
# for the caller's own flags: a leg that narrows which synths it configures has
# to reach this configure too, or the sanitizer build covers a different tree
# from the leg beside it.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

san_dir=${CI_SANITIZER_BUILD_DIR:-${CI_REPO_ROOT}/build-asan}
san_flags="-fsanitize=address,undefined -fno-omit-frame-pointer"

extra=()
while IFS= read -r a; do
	[ -n "$a" ] && extra+=("$a")
done < <(ci_split_args "${CI_CMAKE_ARGS}")

ci_log "configure ${san_dir} with the address and undefined sanitizers"
cmake -S "${CI_REPO_ROOT}" -B "${san_dir}" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_C_FLAGS="${san_flags}" \
	-DCMAKE_CXX_FLAGS="${san_flags}" \
	"${extra[@]}"

ci_log "build ${san_dir} (parallel ${CI_PARALLEL})"
cmake --build "${san_dir}" --parallel "${CI_PARALLEL}"

CI_BUILD_DIR=${san_dir} exec "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/test.sh"
