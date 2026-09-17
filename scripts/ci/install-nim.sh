#!/usr/bin/env bash
# Install the pinned Nim toolchain into CI_NIM_PREFIX and print the PATH entry.
#
# The MCF5407 core compiles its Nim sources during CMake configure and stops the
# configure on any version other than the one its own .nim-version names, so this
# is an exact version and not a range. It matches the `nim-version:` pin the
# GitHub legs give to setup-nim-action; advance the two together.
#
# This exists because the Forgejo jobs run in a plain distribution image rather
# than through a marketplace action. It is the only script here that a GitHub
# workflow does not call.

. "$(cd "${BASH_SOURCE[0]%/*}" && pwd)/lib.sh"

CI_NIM_VERSION=${CI_NIM_VERSION:-2.2.10}
# Outside the workspace on purpose: under it, the compiler tree becomes a subtree
# of the checkout that the build's recursive source globs would walk.
CI_NIM_PREFIX=${CI_NIM_PREFIX:-/opt/nim-${CI_NIM_VERSION}}

if command -v nim >/dev/null 2>&1 &&
   [ "$(nim --version | head -1 | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -1)" = "${CI_NIM_VERSION}" ]; then
	ci_log "Nim ${CI_NIM_VERSION} is already on PATH"
	exit 0
fi

case "$(uname -m)" in
	x86_64|amd64) arch=x64 ;;
	*)
		echo "FAILURE: nim-lang.org publishes Linux binaries for x86_64 only," \
			"and this host is $(uname -m)." >&2
		echo "Install Nim ${CI_NIM_VERSION} by another route and put it on PATH." >&2
		exit 1
		;;
esac

url="https://nim-lang.org/download/nim-${CI_NIM_VERSION}-linux_${arch}.tar.xz"
ci_log "installing Nim ${CI_NIM_VERSION} into ${CI_NIM_PREFIX}"
mkdir -p "${CI_NIM_PREFIX}"
curl -fsSL "${url}" | tar -xJ -C "${CI_NIM_PREFIX}" --strip-components=1

"${CI_NIM_PREFIX}/bin/nim" --version | head -1
have=$("${CI_NIM_PREFIX}/bin/nim" --version | head -1 | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ "${have}" != "${CI_NIM_VERSION}" ]; then
	echo "FAILURE: asked for Nim ${CI_NIM_VERSION}, got ${have}." >&2
	exit 1
fi

# The caller puts this on PATH. A workflow appends it to $GITHUB_PATH; a shell
# can eval the line this prints.
echo "${CI_NIM_PREFIX}/bin"
