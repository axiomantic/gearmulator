#!/usr/bin/env bash
# The system packages the Linux build needs: OpenGL and X for JUCE's GUI layer,
# ALSA for its audio backend, and — only where the host does not already carry
# them — the compiler and build tools a bare container image lacks.
#
# The toolchain half is conditional so that a hosted runner, which already ships
# a newer CMake and Ninja than the distribution packages, keeps the ones it has.
#
# Linux only. On any other host it exits 0 without touching the system, so a
# workflow can call it unconditionally and a macOS developer can run the rest of
# scripts/ci without a guard.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

if [ "$(uname -s)" != "Linux" ]; then
	ci_log "not Linux ($(uname -s)) - nothing to install"
	exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
	echo "FAILURE: this script installs with apt-get, which is not on PATH." >&2
	echo "Install the equivalents of: libgl1-mesa-dev xorg-dev libasound2-dev" >&2
	echo "build-essential cmake ninja-build git" >&2
	exit 1
fi

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

packages=(libgl1-mesa-dev xorg-dev libasound2-dev ca-certificates)
command -v cc    >/dev/null 2>&1 || packages+=(build-essential)
command -v cmake >/dev/null 2>&1 || packages+=(cmake)
command -v ninja >/dev/null 2>&1 || packages+=(ninja-build)
command -v git   >/dev/null 2>&1 || packages+=(git)

ci_log "installing: ${packages[*]}"
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends "${packages[@]}"
