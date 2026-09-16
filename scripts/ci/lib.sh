# Shared settings for the scripts in this directory. Sourced, never executed.
#
# Every knob is an environment variable with a default, so a workflow sets only
# what its matrix leg varies and a human can run the same script with nothing
# set at all.

set -euo pipefail

ci_repo_root() {
	local d
	if d=$(git -C "${BASH_SOURCE[0]%/*}" rev-parse --show-toplevel 2>/dev/null); then
		printf '%s\n' "$d"
		return
	fi
	# A source archive has no .git. Walk up from this file instead.
	(cd "${BASH_SOURCE[0]%/*}/../.." && pwd)
}

CI_REPO_ROOT=${CI_REPO_ROOT:-$(ci_repo_root)}
CI_BUILD_DIR=${CI_BUILD_DIR:-${CI_REPO_ROOT}/build}
CI_BUILD_TYPE=${CI_BUILD_TYPE:-Release}
CI_CMAKE_GENERATOR=${CI_CMAKE_GENERATOR:-}
CI_CMAKE_ARGS=${CI_CMAKE_ARGS:-}
CI_PRESET=${CI_PRESET:-}

# A CTest label regex, not a test-name regex.
CI_TEST_EXCLUDE_LABELS=${CI_TEST_EXCLUDE_LABELS:-IntegrationTest|PluginTest}

if [ -z "${CI_PARALLEL:-}" ]; then
	if command -v nproc >/dev/null 2>&1; then
		CI_PARALLEL=$(nproc)
	elif command -v sysctl >/dev/null 2>&1; then
		CI_PARALLEL=$(sysctl -n hw.ncpu)
	else
		CI_PARALLEL=1
	fi
fi

export CI_REPO_ROOT CI_BUILD_DIR CI_BUILD_TYPE CI_CMAKE_GENERATOR \
	CI_CMAKE_ARGS CI_PRESET CI_PARALLEL CI_TEST_EXCLUDE_LABELS

ci_log() { printf '\n== %s\n' "$*" >&2; }

# CI_CMAKE_ARGS arrives as one string because a workflow matrix cannot carry an
# array. Word-splitting it is the intent, so the split happens in one named
# place rather than at every call site.
ci_split_args() {
	# shellcheck disable=SC2086
	printf '%s\n' ${1-}
}
