#!/usr/bin/env bash
# The system packages the Linux build needs: OpenGL and X for JUCE's GUI layer,
# ALSA for its audio backend, and — only where the host does not already carry
# them — the compiler, build tools and CMake.
#
# The toolchain half is conditional so that a hosted runner, which already ships
# a newer CMake than the distribution packages, keeps the one it has.
#
# Linux only. On any other host it exits 0 without touching the system, so a
# workflow can call it unconditionally and a macOS developer can run the rest of
# scripts/ci without a guard.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

# The floor the TREE needs to configure: the mcf5407 dependency's own
# cmake_minimum_required. It is higher than the 3.20 floor
# require-cmake-version.sh enforces, which is about `ctest --no-tests=error`.
CI_CMAKE_FLOOR_MAJOR=3
CI_CMAKE_FLOOR_MINOR=26

if [ "$(uname -s)" != "Linux" ]; then
	ci_log "not Linux ($(uname -s)) - nothing to install"
	exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
	echo "FAILURE: this script installs with apt-get, which is not on PATH." >&2
	echo "Install the equivalents of: libgl1-mesa-dev xorg-dev libasound2-dev" >&2
	echo "build-essential clang ninja-build git, and CMake" \
		"${CI_CMAKE_FLOOR_MAJOR}.${CI_CMAKE_FLOOR_MINOR} or newer." >&2
	exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

cmake_too_old() {
	command -v cmake >/dev/null 2>&1 || return 0
	local v major minor
	v=$(cmake --version | head -1 | grep -Eo '[0-9]+\.[0-9]+' | head -1)
	major=${v%%.*}; minor=${v##*.}
	[ "${major}" -lt "${CI_CMAKE_FLOOR_MAJOR}" ] ||
		{ [ "${major}" -eq "${CI_CMAKE_FLOOR_MAJOR}" ] &&
		  [ "${minor}" -lt "${CI_CMAKE_FLOOR_MINOR}" ]; }
}

packages=(libgl1-mesa-dev xorg-dev libasound2-dev ca-certificates curl xz-utils)
# build-essential is tested through `make`, not through a compiler: a compiler on
# PATH says nothing about whether CMake has a build program to drive.
command -v make  >/dev/null 2>&1 && command -v c++ >/dev/null 2>&1 || packages+=(build-essential)
command -v ninja >/dev/null 2>&1 || packages+=(ninja-build)
command -v git   >/dev/null 2>&1 || packages+=(git)
# Clang is not the project compiler. The mcf5407 ABI gate reads a C syntax tree
# that only Clang prints, and refuses to configure without it; the alternative it
# offers is turning the gate off, which drops a check this migration must keep.
command -v clang >/dev/null 2>&1 || packages+=(clang)
cmake_too_old && packages+=(cmake)

ci_log "installing: ${packages[*]}"
$SUDO apt-get update
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"

if cmake_too_old; then
	echo "FAILURE: CMake is still older than" \
		"${CI_CMAKE_FLOOR_MAJOR}.${CI_CMAKE_FLOOR_MINOR} after installing." >&2
	cmake --version >&2 || true
	exit 1
fi
