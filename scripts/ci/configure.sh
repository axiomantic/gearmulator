#!/usr/bin/env bash
# CMake configure for every CI leg.
#
# CI_PRESET selects preset mode (what release.yml uses); otherwise the generator,
# build type and extra flags are passed explicitly. Both paths set
# CMAKE_SKIP_INSTALL_ALL_DEPENDENCY.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

if [ -n "${CI_PRESET}" ]; then
	# The preset path is the release build, and it packages with `cpack
	# --preset`. CMAKE_SKIP_INSTALL_ALL_DEPENDENCY is deliberately NOT set
	# here: cpack's preinstall pass is what puts the installed files where
	# the package picks them up, and dropping that edge would ship a package
	# built from whatever the `all` target happened to leave behind.
	ci_log "configure with preset ${CI_PRESET}"
	exec cmake --preset "${CI_PRESET}" -DCMAKE_BUILD_TYPE="${CI_BUILD_TYPE}"
fi

# CMAKE_SKIP_INSTALL_ALL_DEPENDENCY drops the `preinstall: all` edge the Makefile
# generator writes. scripts/pack.cmake runs after a build that has already
# produced everything it packages, so the preinstall pass only re-drives the
# whole build with its output swallowed; on macOS that never returns. Anything
# genuinely missing now fails with a message instead of hanging in silence.
common=(-DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON)

args=(-S "${CI_REPO_ROOT}" -B "${CI_BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CI_BUILD_TYPE}")
[ -n "${CI_CMAKE_GENERATOR}" ] && args+=(-G "${CI_CMAKE_GENERATOR}")

extra=()
while IFS= read -r a; do
	[ -n "$a" ] && extra+=("$a")
done < <(ci_split_args "${CI_CMAKE_ARGS}")

ci_log "configure ${CI_BUILD_DIR} (${CI_BUILD_TYPE}, generator '${CI_CMAKE_GENERATOR:-default}')"
exec cmake "${args[@]}" "${common[@]}" "${extra[@]}"
